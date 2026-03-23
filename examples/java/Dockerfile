FROM ghcr.io/richarah/silex:slim

# Install Java (OpenJDK) and Maven
RUN apk add --no-cache openjdk-21 maven

WORKDIR /workspace
COPY pom.xml ./

# Download dependencies into Maven local repo (cached across builds)
RUN --mount=type=cache,target=/root/.m2/repository \
    mvn dependency:go-offline -q

COPY src ./src

RUN --mount=type=cache,target=/root/.m2/repository \
    mvn package -DskipTests -q

CMD ["java", "-jar", "target/my-app.jar"]
